/* face_glyph.c — GLYPH: the analog face of the Glyph ring.
 *
 * 60 dot ticks hug the bezel (hour dots bigger, 12 o'clock white), two white capsule
 * hands with round caps (true tapered-capsule SDF, anti-aliased), a centre ring, and
 * the second hand as a single red dot riding inside the tick ring, positioned from
 * the millisecond clock so it glides. The date sits in a window at 3 o'clock and moves
 * to 9 (then 6, then 12) when a hand would cover it; the battery, when low or charging,
 * sits under it.
 *
 * Cost: the hands and ticks change once a minute, so per frame only the red dot's
 * ~16 px box repaints (an AA circle at full float precision, 25 ns/px).
 * Build-in: the tick ring fills clockwise dot by dot and the second dot grows; the hands
 * are at the right time from the first frame. Wake from AOD: the outlined AOD hands fill
 * in and the hour-dot ring grows into the full tick ring (4 steps). Minute change: the
 * minute hand clicks forward with a 0.18 s mechanical ease. */
#include "faces/face_kit.h"
#include "nos.h"
#include <stdio.h>

#define R_TICK   218.0f
#define R_SEC    202.0f
#define HOUR_LEN 122.0f
#define MIN_LEN  188.0f
#define TWO_PI   6.2831853f
#define CLICK_S  0.18f

static struct { int last_hm; float t0; fk_wake w; } S = { -1, -100.0f, { 0 } };

static void tick(const face_ctx *c, float dt)
{
    fk_aod_update(c);
    fk_wake_tick(&S.w, c);
    fk_unread_tick(c);
    int hm = c->dt.hour * 60 + c->dt.minute;
    if (hm != S.last_hm) {
        if (S.last_hm >= 0 && (hm - S.last_hm == 1 || (S.last_hm == 1439 && hm == 0))) S.t0 = c->t;
        S.last_hm = hm;
    }
}

/* a hand; core > 0 hollows it (AOD outline, and the wake morph filling it in) */
static void hand(gfx_fb *fb, float a, float len, float hw, uint16_t col, float core, float dx, float dy)
{
    float s = fk_sin(a), c = fk_cos(a);
    float x0 = UI_CX + dx, y0 = UI_CY + dy;
    gfx_taper(fb, x0, y0, x0 + s * len, y0 - c * len, hw, hw, col);
    if (core > 0.25f) gfx_taper(fb, x0, y0, x0 + s * len, y0 - c * len, core, core, C_BG);
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

/* the date window: 3 o'clock unless a hand is there, then 9, 6, 12 */
static void date_slot(float ah, float am, int *x, int *y, int *align)
{
    static const float at[4] = { 1.5708f, 4.7124f, 3.1416f, 0.0f };
    static const int xs[4] = { 388, 78, UI_CX, UI_CX }, ys[4] = { 239, 239, 330, 150 };
    static const int al[4] = { UI_RIGHT, UI_LEFT, UI_CENTER, UI_CENTER };
    const float tol = 0.32f;                               /* about 18 degrees */
    int k = 0;
    for (; k < 3; k++) if (!near_ang(ah, at[k], tol) && !near_ang(am, at[k], tol)) break;
    *x = xs[k]; *y = ys[k]; *align = al[k];
}

static void draw(gfx_fb *fb, const face_ctx *c)
{
    float dx, dy;
    fk_wake_off(&S.w, c, &dx, &dy);
    float k = fk_wake_k(&S.w, c);
    float e = fk_build(&S.w, c);
    float fill = e >= 1.0f ? 60.0f : 60.0f * m_clamp(e / 0.75f, 0, 1);
    ring_ctx rc = { fill, dx, dy, k };
    fk_dots(fb, 60, 5, tick_dot, &rc);

    float ah, am;
    angles(c, &ah, &am);
    float click = (c->t - S.t0) / CLICK_S;
    if (click < 1.0f && !c->aod && e >= 1.0f) {        /* stepping from the previous minute */
        float q = fk_mech(click);
        q = (float)(int)(q * 32.0f) / 32.0f;
        am -= (1.0f - q) * (TWO_PI / 60.0f);
        ah -= (1.0f - q) * (TWO_PI / 720.0f);
    }
    float ux = 0, uy = 28.0f;                          /* AOD / wake: under the 12 tick */
    if (k >= 1.0f) {
        int x, y, al;
        date_slot(ah, am, &x, &y, &al);
        char b[16];
        snprintf(b, sizeof b, "%s %d", ui_weekday3(c->dt.weekday), c->dt.day);
        uint16_t lab = e >= 1.0f ? C_SEC : ui_dim(C_SEC, (float)(int)(fk_ease(e) * 4.0f) / 4.0f);
        int w = ui_label(fb, x, y, b, lab, al);
        fk_status(fb, c, x, y + 24, lab, al);
        /* the unread dot leads the date, so it lives where no hand is */
        int xl = al == UI_RIGHT ? x - w : al == UI_CENTER ? x - w / 2 : x;
        ux = (float)(xl - 14) - UI_CX; uy = (float)(y - 6) - 24.0f;
    }
    /* hands: AOD outlines (2 px) fill in during the wake morph */
    uint16_t hc = ui_mix(C_SEC, C_WHITE, k);
    hand(fb, ah, HOUR_LEN, 7.0f, hc, 5.0f * (1.0f - k), dx, dy);
    hand(fb, am, MIN_LEN, 5.0f, hc, 3.0f * (1.0f - k), dx, dy);
    gfx_fill_circle(fb, UI_CX + dx, UI_CY + dy, 9.0f, hc);
    gfx_fill_circle(fb, UI_CX + dx, UI_CY + dy, 4.5f + 0.5f * (1.0f - k), C_BG);

    if (!c->aod && k >= 1.0f) {
        /* the second hand: one red dot, gliding with the millisecond clock */
        float r = e < 1.0f ? 6.0f * fk_stagger(e, 0.8f, 0.2f) : 6.0f;
        if (r > 0.25f) {
            float as = ((float)c->dt.second + (float)c->dt.millis * 0.001f) / 60.0f * TWO_PI;
            gfx_fill_circle(fb, UI_CX + fk_sin(as) * R_SEC, UI_CY - fk_cos(as) * R_SEC, r, C_RED);
        }
    }
    fk_unread(fb, c, C_PRI, C_BG, dx + ux, dy + uy);
}

const nos_face face_glyph = { "GLYPH", 0, tick, draw, 0 };
