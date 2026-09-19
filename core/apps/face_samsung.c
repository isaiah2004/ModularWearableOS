/* face_samsung.c — "Galaxy" premium analog face in the style of Samsung
 * One UI Watch. A deep charcoal dial with a full 60/12 tick ring and slim
 * silver numerals, elegant tapered silver hour/minute hands with a bevelled
 * sheen, a smooth-sweeping accent second hand, a jewelled centre cap, and two
 * balanced complications: a date pill at 3 o'clock and a battery sub-dial at
 * 9 o'clock. The static dial (ring + numerals + complication frames) is cached
 * in layer tag 11; only the live hands + readouts are drawn each frame. */
#include "apps.h"
#include "../font.h"
#include "../icons.h"
#include "../layer.h"
#include <math.h>
#include <stdio.h>

#define PI 3.14159265f
#define LAY_SAMSUNG 11
#define LAY_SAMSUNG_BASE 12

static const char *WDAY[7] = { "SUN","MON","TUE","WED","THU","FRI","SAT" };
static const char *HNUM[13] = { "","1","2","3","4","5","6","7","8","9","10","11","12" };

/* palette */
#define DIAL_BG   GFX_RGB(14, 15, 18)
#define SILVER    GFX_RGB(226, 230, 238)
#define SILVER_D  GFX_RGB(150, 156, 168)
#define EDGE      GFX_RGB(58, 62, 72)
#define TICK_MIN  GFX_RGB(70, 74, 84)
#define TICK_HR   GFX_RGB(200, 206, 218)
#define NUM_COL   GFX_RGB(205, 210, 222)
#define ACCENT    GFX_RGB(255, 176, 66)   /* warm gold second hand + arcs */
#define FRAME     GFX_RGB(40, 44, 52)
#define BOX_FILL  GFX_RGB(24, 26, 31)

/* ---- geometry helper: point on the dial at radius r (grid units) ---- */
static void polar(float ang, float r, float *x, float *y)
{
    *x = GFX_CX + sinf(ang) * GFX_S(r);
    *y = GFX_CY - cosf(ang) * GFX_S(r);
}

/* ---- numeral centred at (x,y) ---- */
static void num_at(gfx_fb *fb, const font_t *f, float x, float y,
                   const char *s, uint16_t c)
{
    font_draw_centered(fb, f, (int)x, (int)(y + f->ascent * 0.34f), s, c);
}

/* ---- rounded filled box (half extents hw,hh, corner radius r) ---- */
static void fill_round(gfx_fb *fb, float cx, float cy, float hw, float hh,
                       float r, uint16_t col)
{
    gfx_fill_rect(fb, (int)(cx - hw), (int)(cy - hh + r),
                  (int)(2 * hw), (int)(2 * (hh - r)), col);
    gfx_fill_rect(fb, (int)(cx - hw + r), (int)(cy - hh),
                  (int)(2 * (hw - r)), (int)(2 * hh), col);
    gfx_fill_circle(fb, cx - hw + r, cy - hh + r, r, col);
    gfx_fill_circle(fb, cx + hw - r, cy - hh + r, r, col);
    gfx_fill_circle(fb, cx - hw + r, cy + hh - r, r, col);
    gfx_fill_circle(fb, cx + hw - r, cy + hh - r, r, col);
}

/* ---- tapered hand: sample overlapping discs along the axis so the shape
 * gets smooth AA edges. Widest at the base, tapering to a point at the tip and
 * to a slim rounded counterweight at the tail. ---- */
static void hand_taper(gfx_fb *fb, float ang, float r_tail, float r_tip,
                       float w_base, float w_tip, uint16_t col)
{
    float s = sinf(ang), co = -cosf(ang);
    float rt = GFX_S(r_tip), rb = GFX_S(r_tail);
    float hb = GFX_S(w_base) * 0.5f, ht = GFX_S(w_tip) * 0.5f;
    float htail = GFX_S(w_base) * 0.4f * 0.5f;
    /* two tapered segments instead of ~100 fill_circles, so the hand is 2
     * damage commands (a sweep of discs would overflow the recorder). */
    gfx_taper(fb, GFX_CX - s * rb, GFX_CY - co * rb, GFX_CX, GFX_CY, htail, hb, col);
    gfx_taper(fb, GFX_CX, GFX_CY, GFX_CX + s * rt, GFX_CY + co * rt, hb, ht, col);
}

/* full premium hand: dark edge underlay, silver body, bright spine highlight */
static void hand_lux(gfx_fb *fb, float ang, float r_tail, float r_tip,
                     float w_base, float w_tip)
{
    hand_taper(fb, ang, r_tail + 1.5f, r_tip + 1.0f, w_base + 2.2f, w_tip + 1.6f, EDGE);
    hand_taper(fb, ang, r_tail, r_tip, w_base, w_tip, SILVER);
    float s = sinf(ang), co = -cosf(ang);
    float x0, y0, x1, y1;
    polar(ang, r_tip * 0.78f, &x1, &y1);
    x0 = GFX_CX + s * GFX_S(2); y0 = GFX_CY + co * GFX_S(2);
    (void)x0; (void)y0;
    gfx_line(fb, GFX_CX + s * GFX_S(4), GFX_CY + co * GFX_S(4),
             x1, y1, GFX_S(w_tip * 0.6f + 0.8f), GFX_RGB(255, 255, 255));
}

void face_samsung_draw(gfx_fb *fb, const hal_datetime *t)
{
    /* ---------- static dial (cached) ---------- */
    gfx_fb *bg = layer_need_render(LAY_SAMSUNG);
    if (bg) {
        gfx_clear(bg, DIAL_BG);
        /* faint concentric guilloche texture for dial depth */
        for (float r = 34; r <= 68; r += 5.0f)
            gfx_ring(bg, GFX_CX, GFX_CY, GFX_S(r), GFX_S(0.8f), GFX_RGB(24, 26, 31));
        /* subtle framing rings */
        gfx_ring(bg, GFX_CX, GFX_CY, GFX_S(114), GFX_S(2.0f), FRAME);
        gfx_ring(bg, GFX_CX, GFX_CY, GFX_S(72),  GFX_S(1.0f), GFX_RGB(30, 33, 39));

        /* 60 minute ticks + 12 bold hour ticks; 12 marked in accent gold */
        for (int i = 0; i < 60; i++) {
            float a = i * (2 * PI / 60.0f);
            int hr = (i % 5) == 0;
            int top = (i == 0);
            float r0 = top ? 95 : (hr ? 98 : 105), r1 = 111;
            float x0, y0, x1, y1;
            polar(a, r0, &x0, &y0);
            polar(a, r1, &x1, &y1);
            uint16_t tc = top ? ACCENT : (hr ? TICK_HR : TICK_MIN);
            gfx_line(bg, x0, y0, x1, y1, GFX_S(top ? 3.8f : (hr ? 3.2f : 1.2f)), tc);
        }

        /* slim hour numerals (skip 3 & 9 — replaced by complications) */
        for (int h = 1; h <= 12; h++) {
            if (h == 3 || h == 9) continue;
            float a = h * (2 * PI / 12.0f);
            float x, y;
            polar(a, 84, &x, &y);
            num_at(bg, &font_medium, x, y, HNUM[h], NUM_COL);
        }

        /* date pill frame at 3 o'clock */
        float dx, dy;
        polar(3 * (2 * PI / 12.0f), 82, &dx, &dy);
        fill_round(bg, dx, dy, GFX_S(27), GFX_S(15), GFX_S(6), FRAME);
        fill_round(bg, dx, dy, GFX_S(25), GFX_S(13), GFX_S(5), BOX_FILL);

        /* battery sub-dial track at 9 o'clock */
        float bx, by;
        polar(9 * (2 * PI / 12.0f), 82, &bx, &by);
        gfx_fill_circle(bg, bx, by, GFX_S(19), BOX_FILL);
        gfx_ring(bg, bx, by, GFX_S(17), GFX_S(1.4f), FRAME);
        gfx_ring(bg, bx, by, GFX_S(14), GFX_S(2.6f), GFX_RGB(46, 50, 58));
    }
    /* Base = dial + date + battery + hour/minute hands, composited into its own
     * cached layer and re-rendered only when the second ticks. Each frame just
     * blits the base, so the sweeping second hand no longer re-rasterizes the
     * tapered hands + spines every frame. */
    float sec = t->second + t->millis / 1000.0f;   /* smooth: second hand only */
    float min = t->minute + t->second / 60.0f;
    float hr  = (t->hour % 12) + min / 60.0f;
    static int s_last = -1;
    if (t->second != s_last) { s_last = t->second; layer_invalidate(LAY_SAMSUNG_BASE); }
    gfx_fb *base = layer_need_render(LAY_SAMSUNG_BASE);
    if (base) {
        layer_blit(base, LAY_SAMSUNG);             /* dial -> base */
        float dx, dy;
        polar(3 * (2 * PI / 12.0f), 82, &dx, &dy);
        char db[16];
        snprintf(db, sizeof db, "%s %d", WDAY[t->weekday % 7], t->day);
        font_draw_centered(base, &font_small, (int)dx,
                           (int)(dy + font_small.ascent * 0.34f), db, SILVER);
        int batt = hal_battery_percent();
        float bx, by;
        polar(9 * (2 * PI / 12.0f), 82, &bx, &by);
        uint16_t bcol = batt <= 20 ? GFX_RED : ACCENT;
        gfx_arc(base, bx, by, GFX_S(14), GFX_S(2.6f), -2.35f,
                -2.35f + 4.70f * (batt / 100.0f), bcol);
        char bb[8];
        snprintf(bb, sizeof bb, "%d", batt);
        font_draw_centered(base, &font_small, (int)bx,
                           (int)(by + font_small.ascent * 0.34f - GFX_S(2)), bb, SILVER);
        hand_lux(base, hr  * (2 * PI / 12.0f), 16, 54, 9.0f, 2.2f);
        hand_lux(base, min * (2 * PI / 60.0f), 18, 90, 6.6f, 1.6f);
    }
    layer_blit(fb, LAY_SAMSUNG_BASE);

    /* second hand: slim gold with tail counterweight (live, every frame) */
    float sa = sec * (2 * PI / 60.0f);
    float sx, sy, tx, ty;
    polar(sa, 100, &sx, &sy);
    polar(sa + PI, 26, &tx, &ty);
    gfx_line(fb, tx, ty, sx, sy, GFX_S(1.8f), ACCENT);
    polar(sa + PI, 26, &tx, &ty);
    gfx_fill_circle(fb, tx, ty, GFX_S(5.5f), ACCENT);
    gfx_fill_circle(fb, tx, ty, GFX_S(2.6f), DIAL_BG);

    /* ---------- jewelled centre cap ---------- */
    gfx_fill_circle(fb, GFX_CX, GFX_CY, GFX_S(10.5f), EDGE);
    gfx_fill_circle(fb, GFX_CX, GFX_CY, GFX_S(8.0f), SILVER_D);
    gfx_fill_circle(fb, GFX_CX, GFX_CY, GFX_S(5.5f), SILVER);
    gfx_fill_circle(fb, GFX_CX, GFX_CY, GFX_S(2.6f), ACCENT);
}
