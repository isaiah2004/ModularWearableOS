/* face_glyph.c — GLYPH: the analog face of the Glyph ring.
 *
 * 60 dot ticks hug the bezel (hour dots bigger, 12 o'clock white), two white capsule
 * hands with round caps (true tapered-capsule SDF, anti-aliased), a centre ring, and
 * the second hand as a single red dot riding inside the tick ring, positioned from
 * the millisecond clock so it glides. The date sits in a window at 3 o'clock and moves
 * to 9 (then 6, then 12) when a hand would cover it, chosen once a minute with
 * hysteresis and cross-faded (4 steps); the battery, when low or charging, sits under
 * it, and the unread dot leads it (in AOD too, so it never moves).
 *
 * Cost: the hands and ticks change once a minute, so per frame only the red dot's
 * ~16 px box repaints (an AA circle at full float precision, 25 ns/px).
 * Motion (docs/MOTION30.html, whole frames). Build-in: the tick ring fills clockwise, 6
 * dots a frame (frames 0-9), the date fades up in 6 perceptual steps and the second
 * dot snaps in (50/83/100 %); the hands are at the right time from the first frame.
 * Wake from AOD (6 frames): the outlined AOD hands fill in and the hour-dot ring grows
 * into the full tick ring; the date and the second dot fade / grow in over the last 3
 * frames. Minute change: the minute hand clicks forward in 4 frames (40/72/92/100 % of
 * its 20 px tip travel: a click, not a crawl). The second dot glides at 0.7 px a frame
 * (an AA disc at float positions). The date window moves by a 4-frame cross-fade. */
#include "faces/face_kit.h"
#include "nos.h"
#include <stdio.h>

#define R_TICK   218.0f
#define R_SEC    202.0f
#define HOUR_LEN 122.0f
#define MIN_LEN  188.0f
#define TWO_PI   6.2831853f
#define TICK_PER_F 6                 /* build-in: tick dots lit per frame */

static struct {
    int last_hm; nos_time_t t0; fk_wake w;
#ifdef GFX_COST_MODEL
    int pj;                                      /* probe: clock-jump tracker */
#endif
    int slot, slot_from; nos_time_t slot_t0;          /* the date window and its cross-fade */
    fk_xfade date;
} S = { .last_hm = -1, .t0 = -100.0f, .slot = -1, .slot_from = -1, .slot_t0 = -100.0f };

static void angles(const face_ctx *c, float *ah, float *am)
{
    float m = (float)c->dt.minute;
    *am = m / 60.0f * TWO_PI;
    *ah = ((float)(c->dt.hour % 12) + m / 60.0f) / 12.0f * TWO_PI;
}

/* is clock angle `a` within `tol` of `at`? */
static int near_ang(float a, float at, float tol)
{
    float d = a - at;
    while (d > 3.1415927f) d -= TWO_PI;
    while (d < -3.1415927f) d += TWO_PI;
    return d < tol && d > -tol;
}

/* the date windows: 3 o'clock unless a hand is there, then 9, 6, 12 */
static const float k_at[4] = { 1.5708f, 4.7124f, 3.1416f, 0.0f };
static const int k_sx[4] = { 388, 78, UI_CX, UI_CX }, k_sy[4] = { 239, 239, 330, 150 };
static const int k_sal[4] = { UI_RIGHT, UI_LEFT, UI_CENTER, UI_CENTER };

/* pick the window from the minute's resting hands, with hysteresis: the current window
 * is kept until a hand comes within 18 degrees, another one is taken only when both hands
 * are 24 degrees clear (so a hand resting near a boundary can't flicker it) */
static int slot_pick(float ah, float am, int cur)
{
    const float keep = 0.3142f, take = 0.4189f;
    for (int k = 0; k < 3; k++) {
        float tol = k == cur ? keep : take;
        if (!near_ang(ah, k_at[k], tol) && !near_ang(am, k_at[k], tol)) return k;
    }
    for (int k = 0; k < 3; k++)
        if (!near_ang(ah, k_at[k], keep) && !near_ang(am, k_at[k], keep)) return k;
    return 3;
}

/* the minute click's share of the step (1 = home): 4 frames counted from the minute
 * change, which already shows the first 40 % (peak 7.9 px at the tip, last 1.6 px) */
static float click_q(const face_ctx *c, float e)
{
    if (c->aod || e < 1.0f) return 1.0f;
    return mf_at(&FK_TICK4, fk_frames(c->t, S.t0) + 1);
}
/* the date window cross-fade: 4 frames, perceptual ink (in = m_ink of 1/4 .. 1) */
static int slot_k(const face_ctx *c) { return fk_frames(c->t, S.slot_t0) + 1; }
static float slot_in(int k) { return k >= 4 ? 1.0f : m_ink((float)k / 4.0f); }
static float slot_out(int k) { return k >= 4 ? 0.0f : m_ink(1.0f - (float)k / 4.0f); }
/* build-in: labels in 6 perceptual steps; the second dot snaps in at frames 9-11 */
static float lab_k(float e) { return e >= 1.0f ? 1.0f : m_ink((float)(fk_build_k(e) + 1) / 6.0f); }
static float sec_grow(float e) { return e >= 1.0f ? 1.0f : mf_at(&FK_SNAP3, fk_build_k(e) - 8); }

static void tick(const face_ctx *c, float dt)
{
    fk_aod_update(c);
    fk_wake_tick(&S.w, c);
    fk_unread_tick(c);
    int stale = fk_wake_stale(&S.w);
    int hm = c->dt.hour * 60 + c->dt.minute;
    if (hm != S.last_hm) {
        if (S.last_hm >= 0 && !stale && (hm - S.last_hm == 1 || (S.last_hm == 1439 && hm == 0))) S.t0 = c->t;
        S.last_hm = hm;
    }
    if (stale) S.t0 = -100.0f;
    float e = fk_build(&S.w, c);
    int calm = c->aod || stale || e < 1.0f || fk_wake_k(&S.w, c) < 1.0f;
    float ah, am;
    angles(c, &ah, &am);
    int want = slot_pick(ah, am, S.slot);
    if (want != S.slot) {
        S.slot_from = S.slot;
        S.slot_t0 = (calm || S.slot < 0) ? -100.0f : c->t;
        S.slot = want;
    }
    char db[16];
    fk_date(db, c);
    fk_xfade_tick(&S.date, db, c->t, calm);
#ifdef GFX_COST_MODEL
    {
        float k = fk_wake_k(&S.w, c);
        fk_probe_common("glyph", &S.w, c);
        fk_probe_xfade("glyph", "date", &S.date, c->t);
        fk_probe("glyph", "ring.r", MC_MICRO, R_TICK - 4.0f * (1.0f - k));
        /* the minute hand's tip along its arc (the hour's minutes), with the click */
        int fresh = fk_probe_jump(c, &S.pj);
        float am = (float)c->dt.minute / 60.0f * TWO_PI - (1.0f - click_q(c, e)) * (TWO_PI / 60.0f);
        if (c->dt.minute == 0 && c->t == S.t0 && !fresh)     /* the hour wraps: minute 59 is -1 */
            fk_probe("glyph", "min.tip", MC_MICRO, -(TWO_PI / 60.0f) * MIN_LEN);
        fk_probe_at("glyph", "min.tip", MC_MICRO, am * MIN_LEN, fresh);
        if (S.slot_from >= 0) {
            fk_probe("glyph", "slot.in", MC_FADE, 100.0f * slot_in(slot_k(c)));
            fk_probe("glyph", "slot.out", MC_FADE, 100.0f * slot_out(slot_k(c)));
        }
        if (!c->aod) {
            float kl = m_clamp((k - 0.5f) / 0.5f, 0, 1);
            float r = 6.0f * kl * sec_grow(e);
            fk_probe("glyph", "sec.r", MC_MICRO, r);
            if (r > 0.25f) {
                float s = (float)c->dt.second + (float)c->dt.millis * 0.001f;
                fk_probe_at("glyph", "sec", MC_TRACKED, s / 60.0f * TWO_PI * R_SEC, fresh || (c->dt.second == 0 && c->dt.millis < 34));
            }
            fk_probe("glyph", "lab", MC_FADE, 100.0f * lab_k(e));
        }
    }
#endif
}

/* a hand; core > 0 hollows it (AOD outline, and the wake morph filling it in) */
static void hand(gfx_fb *fb, float a, float len, float hw, uint16_t col, float core, float dx, float dy)
{
    float s = fk_sin(a), c = fk_cos(a);
    float x0 = UI_CX + dx, y0 = UI_CY + dy;
    fk_taper(fb, x0, y0, x0 + s * len, y0 - c * len, hw, hw, col);
    if (core > 0.25f) fk_taper(fb, x0, y0, x0 + s * len, y0 - c * len, core, core, C_BG);
}

/* tick ring: dot i of 60. k = wake morph (0 = AOD: hour dots only, smaller, inset) */
typedef struct { float fill, dx, dy, k; } ring_ctx;
static void tick_dot(int i, const void *vc, float *x, float *y, float *d, uint16_t *col)
{
    const ring_ctx *rc = (const ring_ctx *)vc;
    float a = (float)i * (TWO_PI / 60.0f);
    int hour = i % 5 == 0;
    float k = rc->k;
    float r = R_TICK - 4.0f * (1.0f - k);
    *x = UI_CX + rc->dx + fk_sin(a) * r;
    *y = UI_CY + rc->dy - fk_cos(a) * r;
    float f = m_clamp(rc->fill - (float)i, 0, 1);
    f = (float)(int)(f * 4.0f) / 4.0f;
    float full = i == 0 ? 10.0f : hour ? 8.0f : 4.0f;
    uint16_t fc = i == 0 ? C_WHITE : hour ? C_SEC : C_DIS;
    if (k <= 0.0f) { *d = hour ? 5.0f : 0.0f; *col = C_DIS; return; }
    if (k < 1.0f) {
        if (!hour && k < 0.5f) { *d = 0; *col = 0; return; }
        *d = hour ? 5.0f + (full - 5.0f) * k : full; *col = ui_mix(C_DIS, fc, k);
        if (!hour) *col = ui_mix(C_BG, C_DIS, (k - 0.25f) / 0.75f);
        return;
    }
    *d = full * f;
    *col = fc;
}

/* the date (and the battery under it) in window `slot` at opacity a; reports where the
 * unread dot goes: 14 px ahead of the date, on its x-height line */
static void date_at(gfx_fb *fb, const face_ctx *c, int slot, float a, uint16_t dcol, float dx, float dy,
                    float ast, uint16_t stcol, float *ux, float *uy)
{
    int x = k_sx[slot] + (int)dx, y = k_sy[slot] + (int)dy, al = k_sal[slot];
    int w = ui_text_width(UI_F_LABEL, S.date.to, 2);
    if (a > 0.0f) w = fk_xfade_label(fb, &S.date, c->t, x, y, a >= 1.0f ? dcol : ui_dim(dcol, a), al);
    if (ast > 0.0f) fk_status(fb, c, x, y + 24, ui_dim(stcol, ast), al);
    int xl = al == UI_RIGHT ? x - w : al == UI_CENTER ? x - w / 2 : x;
    *ux = (float)(xl - 14) - UI_CX;
    *uy = (float)(y - 6) - 24.0f;
}

static void draw(gfx_fb *fb, const face_ctx *c)
{
    float dx, dy;
    fk_wake_off(&S.w, c, &dx, &dy);
    float k = fk_wake_k(&S.w, c);
    float e = fk_build(&S.w, c);
    float fill = e >= 1.0f ? 60.0f : (float)(TICK_PER_F * (fk_build_k(e) + 1));   /* 6 dots a frame */
    ring_ctx rc = { fill, dx, dy, k };
    fk_dots(fb, 60, 5, tick_dot, &rc);

    float ah, am;
    angles(c, &ah, &am);
    float q = click_q(c, e);
    if (q < 1.0f) {                                    /* clicking on from the previous minute */
        am -= (1.0f - q) * (TWO_PI / 60.0f);
        ah -= (1.0f - q) * (TWO_PI / 720.0f);
    }
    /* the date window: C_DIS in AOD (so the unread dot beside it has a reason to be
     * there), brightening with the wake, cross-fading when the window moves */
    float kl = c->aod ? 0.0f : m_clamp((k - 0.5f) / 0.5f, 0, 1);
    float la = lab_k(e);
    face_ctx ca = *c; ca.aod = 1;
    int st_aod = fk_status_shown(&ca);                 /* critical battery: shown in AOD too */
    float ast = st_aod ? la : m_ink(kl) * la;          /* from black: perceptual steps */
    uint16_t stcol = c->aod ? C_DIS : st_aod ? ui_mix(C_DIS, C_SEC, k) : C_SEC;
    uint16_t dcol = c->aod ? C_DIS : ui_mix(C_DIS, C_SEC, k);
    int slot = S.slot < 0 ? 0 : S.slot;
    int sk = slot_k(c);                                 /* 4 frames, perceptual ink */
    float qi = slot_in(sk), qo = slot_out(sk);
    float ux, uy, ox = 0, oy = 0;
    if (qi < 1.0f && S.slot_from >= 0)
        date_at(fb, c, S.slot_from, la * qo, dcol, dx, dy, ast * qo, stcol, &ox, &oy);
    date_at(fb, c, slot, la * qi, dcol, dx, dy, ast * qi, stcol, &ux, &uy);

    /* hands: AOD outlines (2 px) fill in during the wake morph */
    uint16_t hc = ui_mix(C_SEC, C_WHITE, k);
    hand(fb, ah, HOUR_LEN, 7.0f, hc, 5.0f * (1.0f - k), dx, dy);
    hand(fb, am, MIN_LEN, 5.0f, hc, 3.0f * (1.0f - k), dx, dy);
    gfx_fill_circle(fb, UI_CX + dx, UI_CY + dy, 9.0f, hc);
    gfx_fill_circle(fb, UI_CX + dx, UI_CY + dy, 4.5f + 0.5f * (1.0f - k), C_BG);

    if (!c->aod) {
        /* the second hand: one red dot, gliding with the millisecond clock; it grows in
         * over the build-in's end and the wake's second half */
        float r = 6.0f * kl * sec_grow(e);
        if (r > 0.25f) {
            float as = ((float)c->dt.second + (float)c->dt.millis * 0.001f) / 60.0f * TWO_PI;
            gfx_fill_circle(fb, UI_CX + fk_sin(as) * R_SEC, UI_CY - fk_cos(as) * R_SEC, r, fk_red(c, C_PRI));
        }
    }
    if (qi < 1.0f && S.slot_from >= 0 && !c->aod) {
        fk_unread_a(fb, c, C_PRI, C_BG, ox, oy, qo);
        fk_unread_a(fb, c, C_PRI, C_BG, ux, uy, qi);
    } else
        fk_unread(fb, c, ui_mix(C_SEC, C_PRI, k), C_BG, ux, uy);   /* AOD grey brightening on a wake */
}

const nos_face face_glyph = { "GLYPH", 0, tick, draw, 0 };
