/* face_dot.c — DOT: the house dot-matrix clock.
 *
 * HH over MM in DOT_FONT_TALL on one lattice (pitch 17, dot 0.8 pitch), the unlit
 * cells of every digit box visible as ghost dots. The seconds are one red dot that
 * sits on the same lattice in the gap between the hours and the minutes and blinks
 * once a second. Date above; below, the battery only when it is low or charging.
 *
 * Motion (docs/MOTION30.html, all in whole frames): build-in = the dots of HH (frames
 * 0-7) then MM (2-9) sparkle in (random order) over the grid, the date fades up in 6
 * perceptual ink steps and the red dot snaps in (3 frames); wake from AOD = the thin
 * grey AOD dots grow and brighten into place (6 frames, no blank frame, the AOD drift
 * walks home at 1 px a frame); minute change = dot_text_morph over 8 frames, no glide;
 * the date cross-fades at midnight. 12 h: a lone hour digit is centred; when the hour
 * gains or loses a digit the shared slot steps sideways in whole lattice pitches.
 * Cost: idle frames repaint only the red dot's 16 px box. */
#include "faces/face_kit.h"
#include "nos.h"
#include <stdio.h>
#include <string.h>

#define P      17.0f                 /* lattice pitch */
#define D      (P * 0.8f)            /* lit dot */
#define D_AOD  (P * 0.42f)
#define ROWS   9
#define Y_HH   (UI_CY - P * 0.5f - ROWS * P)
#define Y_MM   (UI_CY + P * 0.5f)
#define Y_DATE 50
#define Y_STAT 428
#define MORPH_N 8                    /* frames */
#define GHOST  0x18C3                /* #181818, neutral */

static struct {
    fk_morph hh, mm;
    fk_wake w;
    fk_xfade date;
} S;

static void enter(void) { S.hh.init = S.mm.init = 0; }

/* the seconds dot: snaps in over the build-in's last frames (50/83/100 %), then blinks */
static float sec_d(const face_ctx *c, float e)
{
    if (e < 1.0f) return D * mf_at(&FK_SNAP3, fk_build_k(e) - 8);
    return D * fk_blink(c);
}
/* the date / battery ink during the build-in: 6 perceptual steps */
static float lab_k(float e)
{
    return e >= 1.0f ? 1.0f : m_ink((float)(fk_build_k(e) + 1) / 6.0f);
}

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
#ifdef GFX_COST_MODEL
    {
        float k = fk_wake_k(&S.w, c);
        fk_probe_common("dot", &S.w, c);
        fk_probe_xfade("dot", "date", &S.date, c->t);
        fk_probe("dot", "wake.d", MC_MICRO, D_AOD + (D - D_AOD) * k);
        if (!c->aod && k >= 1.0f) {
            fk_probe("dot", e < 1.0f ? "sec.in" : "sec", e < 1.0f ? MC_MICRO : MC_DECORATIVE, sec_d(c, e));
            fk_probe("dot", "lab", MC_FADE, 100.0f * lab_k(e));
        }
    }
#endif
}

static void draw_date(gfx_fb *fb, const face_ctx *c, uint16_t col, float dx, float dy)
{
    fk_xfade_label(fb, &S.date, c->t, UI_CX + (int)dx, Y_DATE + (int)dy, col, UI_CENTER);
}

/* One line's minute morph, in place. When the line gains or loses a digit (12 h: "9" ->
 * "10", "12" -> "1") dot_text_morph would glide the shared slot half a glyph sideways on
 * an ease: a dot field sliding at up to 17 px a frame. Instead the shared slot steps in
 * whole lattice pitches (40/72/92/100 % of the 3 pitches, rounded per frame: 1, 2, 3)
 * while its dots morph, and the extra slot morphs to or from blank where it stands. */
static void morph_line(gfx_fb *fb, const dot_style *st, float y, const fk_morph *m, nos_time_t t)
{
    float p = fk_morph_pk(m, t, MORPH_N);
    int la = (int)strlen(m->from), lb = (int)strlen(m->to);
    if (la == lb || p >= 1.0f || la < 1 || lb < 1 || la > 2 || lb > 2) {
        dot_text_morph(fb, st, UI_CX, y, m->from, m->to, 1, p);
        return;
    }
    float xa = UI_CX - (float)dot_text_width(st, m->from) * 0.5f;
    float xb = UI_CX - (float)dot_text_width(st, m->to) * 0.5f;
    float adv = (float)dot_text_width(st, "0") + P;          /* one slot and its gap */
    float cells = (xb - xa) / P;                             /* whole pitches (3) */
    float q = mf_at(&FK_TICK4, fk_frames(t, m->t0) + 1);
    float x0 = xa + P * (float)(int)(cells * q + (cells < 0 ? -0.5f : 0.5f));
    char a0[2] = { m->from[0], 0 }, b0[2] = { m->to[0], 0 };
    dot_text_morph(fb, st, x0, y, a0, b0, 0, p);
    if (lb > la) { char b1[2] = { m->to[1], 0 }; dot_text_morph(fb, st, xb + adv, y, " ", b1, 0, p); }
    else         { char a1[2] = { m->from[1], 0 }; dot_text_morph(fb, st, xa + adv, y, a1, " ", 0, p); }
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
    uint16_t lab = e >= 1.0f ? C_SEC : ui_dim(C_SEC, lab_k(e));
    draw_date(fb, c, lab, 0, 0);
    fk_status(fb, c, UI_CX, Y_STAT, lab, UI_CENTER);

    float g = fk_ghost(c);                              /* lite (picker): the lattice fades out */
    dot_style st = { DOT_FONT_TALL, P, D, C_WHITE, g >= 1.0f ? GHOST : g > 0.0f ? ui_mix(C_BG, GHOST, g) : 0, 1, 1 };
    if (e < 1.0f) {
        /* whole frames: HH over frames 0-7, MM two frames behind (R9) */
        int kb = fk_build_k(e);
        dot_text_anim(fb, &st, UI_CX, Y_HH, S.hh.to, 1, m_clamp((float)(kb + 1) / 8.0f, 0, 1), DOT_REVEAL_RANDOM, 7);
        dot_text_anim(fb, &st, UI_CX, Y_MM, S.mm.to, 1, m_clamp((float)(kb - 1) / 8.0f, 0, 1), DOT_REVEAL_RANDOM, 11);
    } else {
        morph_line(fb, &st, Y_HH, &S.hh, c->t);
        morph_line(fb, &st, Y_MM, &S.mm, c->t);
    }
    /* seconds: one red lattice dot between HH and MM */
    float d = sec_d(c, e);
    if (d > 0.5f) dot_disc(fb, UI_CX, UI_CY, d, fk_red(c, C_PRI));
    fk_unread(fb, c, C_PRI, C_BG, 0, 0);
}

const nos_face face_dot = { "DOT", enter, tick, draw, 0 };
