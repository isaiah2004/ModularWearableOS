/* face_dense.c — "Infograph": editorial split layout. A cropped, edge-faded
 * rotating Earth sits on the right; a bold stacked clock (HH / MM), day-of-week,
 * and dd/MMM date run down the left, with a battery arc + bolt at bottom-centre.
 * The Earth is 30 keyframes cross-faded and PING-PONGED (forward then back) so
 * the visible front half plays at double density with no dead back-side. */
#include "apps.h"
#include "../wos.h"
#include "../font.h"
#include "../icons.h"
#include "../battery.h"
#include "../settings.h"
#include "../earth_sprite.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265f

static const char *WDAY[7] = { "SUN","MON","TUE","WED","THU","FRI","SAT" };
static const char *MON[12] = { "JAN","FEB","MAR","APR","MAY","JUN",
                               "JUL","AUG","SEP","OCT","NOV","DEC" };

static float    s_earth_ph = 15.0f;   /* begin on the bright day side */
static uint32_t s_last;
static uint16_t *s_earth_ram;          /* PSRAM copy of the sprite (flash is slow) */

/* Gamma-correct cross-fade LUTs: linearise 5/6-bit channels to 0-255 light,
 * blend in linear space, re-encode. Blending in gamma space darkens the
 * mid-fade (the pulsing "dip"); doing it in light removes it. */
static uint8_t s_lin5[32], s_lin6[64], s_gam5[256], s_gam6[256];
static int     s_lut_ready;

/* Gamma-correct cross-fade for the low/mid brightness range where the fade is
 * used (above ~75% we snap keyframes instead). Fixed 1.86 = flat mid-fade there;
 * the panel's high-brightness gamma is non-monotonic and not worth chasing. */
#define EARTH_GAMMA 1.86f
static void earth_lut_build(void)
{
    for (int i = 0; i < 32; i++) s_lin5[i] = (uint8_t)(powf(i / 31.0f, EARTH_GAMMA) * 255.0f + 0.5f);
    for (int i = 0; i < 64; i++) s_lin6[i] = (uint8_t)(powf(i / 63.0f, EARTH_GAMMA) * 255.0f + 0.5f);
    for (int L = 0; L < 256; L++) {
        s_gam5[L] = (uint8_t)(powf(L / 255.0f, 1.0f / EARTH_GAMMA) * 31.0f + 0.5f);
        s_gam6[L] = (uint8_t)(powf(L / 255.0f, 1.0f / EARTH_GAMMA) * 63.0f + 0.5f);
    }
    s_lut_ready = 1;
}

static inline uint16_t earth_blend(uint16_t a, uint16_t b, int t)
{
    int it = 255 - t;
    int r  = (s_lin5[a >> 11]        * it + s_lin5[b >> 11]        * t) >> 8;
    int g  = (s_lin6[(a >> 5) & 63]  * it + s_lin6[(b >> 5) & 63]  * t) >> 8;
    int bl = (s_lin5[a & 31]         * it + s_lin5[b & 31]         * t) >> 8;
    return (uint16_t)((s_gam5[r] << 11) | (s_gam6[g] << 5) | s_gam5[bl]);
}

/* faux-bold, left-aligned at x: draw twice with a 1px offset to thicken */
static void bold_text(gfx_fb *fb, const font_t *f, int x, int y,
                      const char *s, uint16_t col)
{
    font_draw(fb, f, x, y, s, col);
    font_draw(fb, f, x + 1, y, s, col);
}

/* Cross-faded Earth (fa->fb2 by alpha) centred at (ecx,ecy), native size. Edges
 * are pre-faded to black, so an opaque blit melts into the background. */
static void draw_earth(gfx_fb *fb, int ecx, int ecy, int fa, int fb2, uint8_t alpha)
{
    int s = EARTH_SPRITE_SIZE;
    int x0 = ecx - s / 2, y0 = ecy - s / 2;
    if (fb->rec) {
        gfx_rec_cmd(fb, x0, y0, x0 + s, y0 + s,
                    gfx_hash_u(gfx_hash_u(gfx_hash_u(0x300u, (unsigned)ecx),
                    (unsigned)fa), (unsigned)alpha));
        return;
    }
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (c0 < 0) c0 = 0;
    if (c1 > GFX_W) c1 = GFX_W;
    int fsz = EARTH_SPRITE_SIZE * EARTH_SPRITE_SIZE;
    const uint16_t *A = s_earth_ram ? s_earth_ram + fa * fsz : earth_sprite[fa];
    const uint16_t *B = s_earth_ram ? s_earth_ram + fb2 * fsz : earth_sprite[fb2];
    /* Near a keyframe just copy one frame (single read); only mid-transition
     * pays for the two-frame blend. */
    int mode = alpha < 10 ? 0 : alpha > 245 ? 1 : 2;
    for (int py = 0; py < fb->h; py++) {
        int y = py + fb->y_base;
        int ey = y - y0;
        if ((unsigned)ey >= (unsigned)s) continue;
        uint16_t *dst = &fb->px[py * GFX_W];
        const uint16_t *ra = &A[ey * s], *rb = &B[ey * s];
        int lo = x0 < c0 ? c0 : x0;
        int hi = (x0 + s) > c1 ? c1 : (x0 + s);
        if (hi > GFX_W) hi = GFX_W;
        if (mode == 0)
            memcpy(&dst[lo], &ra[lo - x0], (size_t)(hi - lo) * 2);
        else if (mode == 1)
            memcpy(&dst[lo], &rb[lo - x0], (size_t)(hi - lo) * 2);
        else
            for (int dx = lo; dx < hi; dx++) { int ex = dx - x0; dst[dx] = earth_blend(ra[ex], rb[ex], alpha); }
    }
}

/* battery arc + bolt: a colour-coded ring filled to the charge level with a
 * lightning bolt in the middle */
static void battery_ring(gfx_fb *fb, float cx, float cy, int pct)
{
    float r = GFX_S(17), th = GFX_S(3.4f);
    uint16_t col = pct < 25 ? GFX_RED : pct < 50 ? GFX_ORANGE : GFX_GREEN;
    gfx_ring(fb, cx, cy, r, th, GFX_RGB(40, 42, 48));
    if (pct > 1) gfx_arc(fb, cx, cy, r, th, 0, pct / 100.0f * 2 * PI, col);
    icon_glyph(fb, ICON_FLASH, cx, cy, GFX_S(9), col);
}

void face_dense_draw(gfx_fb *fb, const hal_datetime *t)
{
    static uint32_t s_fid = 0xFFFFFFFFu;
    static int s_batt, s_fa, s_fb2;
    static uint8_t s_alpha;
    static char s_hh[4], s_mm[4], s_wd[4], s_date[12];

    if (wos_frame_id != s_fid) {
        s_fid = wos_frame_id;
        if (!s_lut_ready) earth_lut_build();
        if (!s_earth_ram) {                    /* one-time: stage the sprite in PSRAM */
            uint32_t bytes = (uint32_t)EARTH_SPRITE_FRAMES * EARTH_SPRITE_SIZE
                             * EARTH_SPRITE_SIZE * 2u;
            s_earth_ram = (uint16_t *)hal_big_alloc(bytes);
            if (s_earth_ram) memcpy(s_earth_ram, earth_sprite, bytes);
        }
        uint32_t now = hal_millis();
        float dt = s_last ? (now - s_last) / 1000.0f : 0.016f;
        if (dt <= 0 || dt > 0.5f) dt = 0.016f;
        s_last = now;
        /* Ping-pong the bright hump: the sprite is dim -> bright(peak) -> dim (the
         * dark trough is cut out), so play forward then backward. Both turnarounds
         * (frame 0 and frame N-1) land in the DIM, hiding the reversal. */
        s_earth_ph += dt * 1.4f;
        float period = 2.0f * (EARTH_SPRITE_FRAMES - 1);
        float ph = fmodf(s_earth_ph, period);
        float pos = ph <= (EARTH_SPRITE_FRAMES - 1) ? ph : (period - ph);
        s_fa = (int)pos;
        if (s_fa > EARTH_SPRITE_FRAMES - 2) s_fa = EARTH_SPRITE_FRAMES - 2;
        s_fb2 = s_fa + 1;
        /* Linear cross-fade = constant motion velocity (no pulsing). Any easing
         * varies velocity within each keyframe interval and reads as pulsing. */
        float frac = pos - (float)s_fa;
        s_alpha = (uint8_t)(frac * 255.0f);
        /* Above ~75% brightness the panel steepens/switches its own gamma and the
         * cross-fade mid-point dips; snap to the nearest keyframe there so there
         * is no mid-fade to distort (trades smoothness for a stepped globe). */
        if (g_brightness > 191)
            s_alpha = s_alpha < 128 ? 0 : 255;

        s_batt = battery_pct();
        snprintf(s_hh, sizeof s_hh, "%02d", t->hour);
        snprintf(s_mm, sizeof s_mm, "%02d", t->minute);
        snprintf(s_wd, sizeof s_wd, "%s", WDAY[t->weekday % 7]);
        snprintf(s_date, sizeof s_date, "%d/%s", t->day, MON[(t->month - 1) % 12]);
    }

    gfx_clear(fb, GFX_BLACK);
    draw_earth(fb, (int)(GFX_CX + GFX_S(50)), (int)GFX_CY, s_fa, s_fb2, s_alpha);

    /* left column (left-aligned): big tight stacked clock + day + dd/MMM date.
     * Dedicated hero fonts (~15% up); shifted right off the far bezel. Lower
     * rows are indented more to clear the round screen's bottom-left curve. */
    bold_text(fb, &font_clock, (int)GFX_S(24), (int)(GFX_CY - GFX_S(40)), s_hh, GFX_WHITE);
    bold_text(fb, &font_clock, (int)GFX_S(24), (int)(GFX_CY + GFX_S(14)), s_mm, GFX_ACCENT);
    bold_text(fb, &font_dow, (int)GFX_S(30), (int)(GFX_CY + GFX_S(50)), s_wd, GFX_WHITE);
    bold_text(fb, &font_date, (int)GFX_S(38), (int)(GFX_CY + GFX_S(74)), s_date, GFX_GREY);

    /* battery arc + bolt, bottom-centre */
    battery_ring(fb, GFX_CX, GFX_CY + GFX_S(98), s_batt);
}
