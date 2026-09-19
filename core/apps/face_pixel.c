/* face_pixel.c — "Pixel", a Google Pixel Watch / Material You inspired face.
 * Big friendly stacked time, a thick vibrant seconds arc with rounded caps
 * hugging the bezel, and two Material "chips" (pills) for the date and steps.
 * Bold, colorful, breathable — one strong accent on a dark tinted ground. */
#include "apps.h"
#include "../font.h"
#include "../icons.h"
#include <math.h>
#include <stdio.h>

#define PI 3.14159265f

static const char *WDAY[7] = { "SUN","MON","TUE","WED","THU","FRI","SAT" };

/* Material palette (custom, on a near-black ground) */
#define COL_BG     GFX_RGB(14, 16, 20)
#define COL_TRACK  GFX_RGB(34, 37, 44)
#define COL_HERO   GFX_RGB(126, 214, 138)   /* Material green — the hero accent */
#define COL_CHIP   GFX_RGB(30, 34, 42)       /* chip fill */

/* Seconds sweep split into a "settled" arc (snapped to the last whole second,
 * so it only changes once per second) and a small sub-second "tip" (changes
 * every frame). With retained-mode damage the tip repaints each frame (a tiny
 * wedge) and the settled arc only on the second tick — 2 arcs total, so the
 * (expensive) trig setup stays cheap. Rounded caps at both ends. */
static void seg_sweep(gfx_fb *fb, float cx, float cy, float r, float th,
                      float sec, uint16_t hero, uint16_t track)
{
    (void)track;
    float step = 2 * PI / 60.0f;
    float wa = (int)sec * step, tip = sec * step, cr = th * 0.5f;
    if (wa > 0.0002f)             gfx_arc(fb, cx, cy, r, th, 0.0f, wa, hero);
    if (tip > wa + 0.0002f)       gfx_arc(fb, cx, cy, r, th, wa, tip, hero);
    gfx_fill_circle(fb, cx, cy - r, cr, hero);                              /* start cap */
    gfx_fill_circle(fb, cx + sinf(tip) * r, cy - cosf(tip) * r, cr, hero);  /* tip cap */
}

/* Material pill (rounded-rect) filled with `col`, centered at (cx,cy) */
static void pill(gfx_fb *fb, float cx, float cy, float w, float h, uint16_t col)
{
    float r = h * 0.5f;
    gfx_fill_rect(fb, (int)(cx - w * 0.5f + r), (int)(cy - h * 0.5f),
                  (int)(w - 2 * r), (int)h, col);
    gfx_fill_circle(fb, cx - w * 0.5f + r, cy, r, col);
    gfx_fill_circle(fb, cx + w * 0.5f - r, cy, r, col);
}

void face_pixel_draw(gfx_fb *fb, const hal_datetime *t)
{
    gfx_clear(fb, COL_BG);

    /* ---- hero seconds arc, hugging the bezel with rounded caps ---- */
    float sec = t->second + t->millis / 1000.0f;
    float rr = GFX_S(111), th = GFX_S(13);
    gfx_ring(fb, GFX_CX, GFX_CY, rr, th, COL_TRACK);
    seg_sweep(fb, GFX_CX, GFX_CY, rr, th, sec, COL_HERO, COL_TRACK);

    /* ---- date chip (top) ---- */
    char d[16];
    snprintf(d, sizeof d, "%s %d", WDAY[t->weekday % 7], t->day);
    pill(fb, GFX_CX, GFX_S(44), GFX_S(74), GFX_S(30), COL_CHIP);
    font_draw_centered(fb, &font_small, (int)GFX_CX,
                       (int)(GFX_S(44) + GFX_S(5)), d, COL_HERO);

    /* ---- big stacked time HH / MM ---- */
    char hh[4], mm[4];
    snprintf(hh, sizeof hh, "%02d", t->hour);
    snprintf(mm, sizeof mm, "%02d", t->minute);
    font_draw_centered(fb, &font_big, (int)GFX_CX, (int)(GFX_CY - GFX_S(6)),
                       hh, GFX_WHITE);
    font_draw_centered(fb, &font_big, (int)GFX_CX, (int)(GFX_CY + GFX_S(38)),
                       mm, COL_HERO);

    /* ---- steps chip (bottom) ---- */
    int steps = hal_step_count();
    char s[24];
    if (steps >= 1000) snprintf(s, sizeof s, "%d.%dk steps", steps / 1000, (steps % 1000) / 100);
    else               snprintf(s, sizeof s, "%d steps", steps);
    pill(fb, GFX_CX, GFX_S(196), GFX_S(96), GFX_S(30), COL_CHIP);
    font_draw_centered(fb, &font_small, (int)GFX_CX,
                       (int)(GFX_S(196) + GFX_S(5)), s, GFX_WHITE);
}
