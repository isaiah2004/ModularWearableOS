/* face_nothing.c — "NOTHING", a monochrome technical watch face in the style
 * of Nothing OS / CMF: pure black, white + greys, a SINGLE red accent.
 *
 * Composition (strict grid, generous negative space):
 *   - a perimeter ring of 60 small DOTS as minute markers (hour dots brighter,
 *     quarters brightest) — cached as a static layer;
 *   - one big minimal 24h time dead-centre (font_big, tabular);
 *   - a single RED dot orbiting the dot-ring as the live seconds indicator —
 *     the only colour on the face;
 *   - tiny letter-spaced technical caps: date on top, a short rule, a place
 *     name and a fake coordinate below, plus a compact battery readout.
 *
 * Exposes: void face_nothing_draw(gfx_fb *fb, const hal_datetime *t);
 */
#include "apps.h"
#include "../font.h"
#include "../layer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265f
#define LAY_NOTHING 10          /* unique static-layer tag for this face */

#define RING_R   112            /* dot-ring radius on the 240 grid */

static const char *WDAY[7] = { "SUN","MON","TUE","WED","THU","FRI","SAT" };
static const char *MON[12] = { "JAN","FEB","MAR","APR","MAY","JUN",
                               "JUL","AUG","SEP","OCT","NOV","DEC" };

/* Letter-spaced small caps, centred on cx. Nothing labels breathe: we insert a
 * fixed gap (grid units) between glyphs so short technical labels read wide and
 * engineered rather than cramped. */
static void caps(gfx_fb *fb, int cx, int y, const char *s, uint16_t col, float gap)
{
    const font_t *f = &font_small;
    int g = (int)GFX_S(gap);
    int total = 0;
    for (const char *c = s; *c; c++)
        total += font_text_width(f, (char[]){*c, 0}) + (c[1] ? g : 0);
    int x = cx - total / 2;
    for (const char *c = s; *c; c++) {
        char one[2] = { *c, 0 };
        x = font_draw(fb, f, x, y, one, col);
        if (c[1]) x += g;
    }
}

void face_nothing_draw(gfx_fb *fb, const hal_datetime *t)
{
    /* ---------- static background: the 60-dot minute ring (cached) ---------- */
    gfx_fb *bg = layer_need_render(LAY_NOTHING);
    if (bg) {
        gfx_clear(bg, GFX_BLACK);
        for (int i = 0; i < 60; i++) {
            float a = i * (2 * PI / 60.0f);
            float x = GFX_CX + sinf(a) * GFX_S(RING_R);
            float y = GFX_CY - cosf(a) * GFX_S(RING_R);
            if (i % 15 == 0)                     /* 12 / 3 / 6 / 9 */
                gfx_fill_circle(bg, x, y, GFX_S(3.0f), GFX_RGB(215, 215, 220));
            else if (i % 5 == 0)                 /* hour markers */
                gfx_fill_circle(bg, x, y, GFX_S(2.0f), GFX_RGB(130, 130, 136));
            else                                 /* minute ticks */
                gfx_fill_circle(bg, x, y, GFX_S(1.1f), GFX_RGB(46, 46, 50));
        }
    }
    layer_blit(fb, LAY_NOTHING);

    /* ---------- top: date, letter-spaced ---------- */
    char b[64];
    snprintf(b, sizeof b, "%s %02d %s", WDAY[t->weekday % 7], t->day,
             MON[(t->month - 1) % 12]);
    caps(fb, (int)GFX_CX, (int)GFX_S(62), b, GFX_GREY, 3.0f);

    /* ---------- centre: the big minimal time ---------- */
    snprintf(b, sizeof b, "%02d:%02d", t->hour, t->minute);
    font_draw_centered(fb, &font_big, (int)GFX_CX,
                       (int)(GFX_CY + font_big.ascent * 0.34f), b, GFX_WHITE);

    /* ---------- a short technical rule under the time ---------- */
    gfx_line(fb, GFX_CX - GFX_S(20), GFX_S(150), GFX_CX + GFX_S(20), GFX_S(150),
             GFX_S(1.3f), GFX_RGB(70, 70, 76));

    /* ---------- below: place (IP-derived), then coordinate + battery on one line ---------- */
    char city[24]; float lat = 0, lon = 0;
    if (hal_location(city, sizeof city, &lat, &lon)) {
        for (char *p = city; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;   /* uppercase */
        caps(fb, (int)GFX_CX, (int)GFX_S(168), city, GFX_RGB(155, 155, 161), 3.5f);
        int la = (int)(lat * 100.0f), lo = (int)(lon * 100.0f);
        char ns = la < 0 ? 'S' : 'N', ew = lo < 0 ? 'W' : 'E';
        if (la < 0) la = -la;
        if (lo < 0) lo = -lo;
        snprintf(b, sizeof b, "%d.%02d%c  %d.%02d%c   BAT %d",
                 la / 100, la % 100, ns, lo / 100, lo % 100, ew, hal_battery_percent());
    } else {
        caps(fb, (int)GFX_CX, (int)GFX_S(168), "NO LOCATION", GFX_RGB(120, 120, 126), 3.0f);
        snprintf(b, sizeof b, "BAT %d", hal_battery_percent());
    }
    caps(fb, (int)GFX_CX, (int)GFX_S(188), b, GFX_RGB(82, 82, 88), 1.6f);

    /* ---------- the single red accent: seconds dot orbiting the ring ------- */
    float sec = t->second + t->millis / 1000.0f;
    float a = sec * (2 * PI / 60.0f);
    float sx = GFX_CX + sinf(a) * GFX_S(RING_R);
    float sy = GFX_CY - cosf(a) * GFX_S(RING_R);
    gfx_fill_circle(fb, sx, sy, GFX_S(3.6f), GFX_RED);
}
