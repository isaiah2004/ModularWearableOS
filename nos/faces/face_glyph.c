/* face_glyph.c — GLYPH: the analog face of the Glyph ring.
 *
 * 60 dot ticks hug the bezel (hour dots bigger and brighter), two white capsule
 * hands with round caps (true tapered-capsule SDF, anti-aliased), a centre ring, and
 * the second hand as a single red dot riding inside the tick ring, positioned from
 * the millisecond clock so it glides.
 *
 * Cost: the hands and ticks only change once a minute, so per frame only the red
 * dot's ~16 px box repaints (dot_disc records exactly its own bbox). Full redraw
 * (pager slide) is two dot rings + two tapers + three discs.
 * Build-in: the tick ring fills clockwise dot by dot while both hands sweep from
 * 12 o'clock into place (EASE_STANDARD), then the second dot appears. */
#include "faces/face_kit.h"
#include "nos.h"
#include <stdio.h>

#define R_TICK   218.0f
#define R_SEC    202.0f
#define HOUR_LEN 122.0f
#define MIN_LEN  188.0f
#define TWO_PI   6.2831853f

static void hand(gfx_fb *fb, float a, float len, float hw, uint16_t col)
{
    float s = fk_sin(a), c = fk_cos(a);
    gfx_taper(fb, UI_CX, UI_CY, UI_CX + s * len, UI_CY - c * len, hw, hw, col);
}

static void hand_outline(gfx_fb *fb, float a, float len, float hw, float t, uint16_t col, int dx, int dy)
{
    float s = fk_sin(a), c = fk_cos(a);
    float x0 = UI_CX + dx, y0 = UI_CY + dy;
    gfx_taper(fb, x0, y0, x0 + s * len, y0 - c * len, hw, hw, col);
    gfx_taper(fb, x0, y0, x0 + s * len, y0 - c * len, hw - t, hw - t, C_BG);
}

static void tick(const face_ctx *c, float dt) { fk_aod_update(c); }

/* tick ring: dot i of 60; hour dots bigger and brighter. ctx = fill (dots lit so far) */
typedef struct { float fill, dx, dy; int aod; } ring_ctx;
static void tick_dot(int i, const void *vc, float *x, float *y, float *d, uint16_t *col)
{
    const ring_ctx *rc = (const ring_ctx *)vc;
    float a = (float)i * (TWO_PI / 60.0f);
    int hour = i % 5 == 0;
    float r = rc->aod ? R_TICK - 4 : R_TICK;
    *x = UI_CX + rc->dx + fk_sin(a) * r;
    *y = UI_CY + rc->dy - fk_cos(a) * r;
    if (rc->aod) { *d = hour ? 5.0f : 0.0f; *col = C_DIS; return; }
    float f = m_clamp(rc->fill - (float)i, 0, 1);
    f = (float)(int)(f * 4.0f) / 4.0f;
    *d = (hour ? 8.0f : 4.0f) * f;
    *col = hour ? C_SEC : C_DIS;
}

static void angles(const face_ctx *c, float *ah, float *am)
{
    float m = (float)c->dt.minute;
    *am = m / 60.0f * TWO_PI;
    *ah = ((float)(c->dt.hour % 12) + m / 60.0f) / 12.0f * TWO_PI;
}

static void draw_aod(gfx_fb *fb, const face_ctx *c)
{
    int dx = fk_aod_dx, dy = fk_aod_dy;
    float ah, am;
    angles(c, &ah, &am);
    ring_ctx rc = { 60.0f, (float)dx, (float)dy, 1 };
    fk_dots(fb, 60, 5, tick_dot, &rc);
    hand_outline(fb, ah, HOUR_LEN, 7.0f, 2.0f, C_SEC, dx, dy);
    hand_outline(fb, am, MIN_LEN, 5.0f, 2.0f, C_SEC, dx, dy);
    gfx_fill_circle(fb, UI_CX + dx, UI_CY + dy, 9.0f, C_SEC);
    gfx_fill_circle(fb, UI_CX + dx, UI_CY + dy, 5.0f, C_BG);
}

static void draw(gfx_fb *fb, const face_ctx *c)
{
    if (c->aod) { draw_aod(fb, c); return; }
    float e = c->enter;
    /* tick ring: minute dots, then the hour dots over them */
    float fill = e >= 1.0f ? 60.0f : 60.0f * m_clamp(e / 0.75f, 0, 1);
    ring_ctx rc = { fill, 0, 0, 0 };
    fk_dots(fb, 60, 5, tick_dot, &rc);

    char b[16];
    snprintf(b, sizeof b, "%s %d", ui_weekday3(c->dt.weekday), c->dt.day);
    ui_label(fb, UI_CX, 330, b, e >= 1.0f ? C_SEC : ui_dim(C_SEC, (float)(int)(fk_ease(e) * 4.0f) / 4.0f), UI_CENTER);

    float ah, am;
    angles(c, &ah, &am);
    if (e < 1.0f) {
        /* both hands swing clockwise into place from a quarter / a third behind */
        float k = fk_ease(e / 0.8f);
        k = (float)(int)(k * 256.0f) / 256.0f;
        ah -= (1.0f - k) * 1.5708f;
        am -= (1.0f - k) * 2.0944f;
    }
    hand(fb, ah, HOUR_LEN, 7.0f, C_PRI);
    hand(fb, am, MIN_LEN, 5.0f, C_WHITE);
    gfx_fill_circle(fb, UI_CX, UI_CY, 9.0f, C_WHITE);
    gfx_fill_circle(fb, UI_CX, UI_CY, 4.5f, C_BG);

    /* the second hand: one red dot, gliding with the millisecond clock */
    float d = e < 1.0f ? 12.0f * fk_stagger(e, 0.8f, 0.2f) : 12.0f;
    if (d > 0.5f) {
        float as = ((float)c->dt.second + (float)c->dt.millis * 0.001f) / 60.0f * TWO_PI;
        dot_disc(fb, UI_CX + fk_sin(as) * R_SEC, UI_CY - fk_cos(as) * R_SEC, d, C_RED);
    }
}

const nos_face face_glyph = { "GLYPH", 0, tick, draw, 0 };
